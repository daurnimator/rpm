/** \ingroup rpmio
 * \file rpmio/rpmsq.c
 */

#include "system.h"
                                                                                
#if defined(HAVE_PTHREAD_H) && !defined(__LCLINT__)

#include <pthread.h>

#define	DO_LOCK()	pthread_mutex_lock(&rpmsigTbl_lock);
#define	DO_UNLOCK()	pthread_mutex_unlock(&rpmsigTbl_lock);
#define	INIT_LOCK()	\
     {	pthread_mutexattr_t attr; \
	pthread_mutexattr_init(&attr); \
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE); \
	pthread_mutex_init (&rpmsigTbl_lock, &attr); \
	pthread_mutexattr_destroy(&attr); \
	rpmsigTbl_sigchld->active = 0; \
     }
#define	ADD_REF(__tbl)	(__tbl)->active++
#define	SUB_REF(__tbl)	--(__tbl)->active
#define	CLEANUP_HANDLER(__handler, __arg, __oldtypeptr) \
	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, (__oldtypeptr)); \
	pthread_cleanup_push((__handler), (__arg));
#define	CLEANUP_RESET(__execute, __oldtype) \
	pthread_cleanup_pop(__execute); \
	pthread_setcanceltype ((__oldtype), &(__oldtype));

#define	SAME_THREAD(_a, _b)	pthread_equal(((pthread_t)_a), ((pthread_t)_b))

#define	ME()	((void *)pthread_self())

#else

#define	DO_LOCK()
#define	DO_UNLOCK()
#define	INIT_LOCK()
#define	ADD_REF(__tbl)
#define	SUB_REF(__tbl)
#define	CLEANUP_HANDLER(__handler, __arg, __oldtypeptr)
#define	CLEANUP_RESET(__execute, __oldtype)

#define	SAME_THREAD(_a, _b)	(42)

#define	ME()	(((void *))getpid())

#endif	/* HAVE_PTHREAD_H */

#include <rpmsq.h>

#include "debug.h"

#define	_RPMSQ_DEBUG	0
/*@unchecked@*/
int _rpmsq_debug = _RPMSQ_DEBUG;

/*@unchecked@*/
static struct rpmsqElem rpmsqRock;
/*@unchecked@*/
rpmsq rpmsqQueue = &rpmsqRock;

int rpmsqInsert(void * elem, void * prev)
{
    rpmsq sq = (rpmsq) elem;
    int ret = -1;

    if (sq != NULL) {
#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "    Insert(%p): %p\n", ME(), sq);
/*@=modfilesys@*/
#endif
	ret = sighold(SIGCHLD);
	if (ret == 0) {
	    sq->child = 0;
	    sq->reaped = 0;
	    sq->status = 0;
	    sq->reaper = 1;
	    sq->pipes[0] = sq->pipes[1] = -1;

	    sq->id = ME();
	    ret = pthread_mutex_init(&sq->mutex, NULL);
	    ret = pthread_cond_init(&sq->cond, NULL);
	    insque(elem, (prev ? prev : rpmsqQueue));
	    ret = sigrelse(SIGCHLD);
	}
    }
    return ret;
}

int rpmsqRemove(void * elem)
{
    rpmsq sq = (rpmsq) elem;
    int ret = -1;

    if (elem != NULL) {

#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "    Remove(%p): %p\n", ME(), sq);
/*@=modfilesys@*/
#endif
	ret = sighold (SIGCHLD);
	if (ret == 0) {
	    remque(elem);
	    ret = pthread_cond_destroy(&sq->cond);
	    ret = pthread_mutex_destroy(&sq->mutex);
	    sq->id = NULL;
	    if (sq->pipes[1])	close(sq->pipes[1]);
	    if (sq->pipes[0])	close(sq->pipes[0]);
	    sq->pipes[0] = sq->pipes[1] = -1;
#ifdef	NOTYET	/* rpmpsmWait debugging message needs */
	    sq->reaper = 1;
	    sq->status = 0;
	    sq->reaped = 0;
	    sq->child = 0;
#endif
	    ret = sigrelse(SIGCHLD);
	}
    }
    return ret;
}

/*@unchecked@*/
sigset_t rpmsqCaught;

/*@unchecked@*/
static pthread_mutex_t rpmsigTbl_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/*@unchecked@*/
/*@-fullinitblock@*/
static struct rpmsig_s {
    int signum;
    void (*handler) (int signum, siginfo_t * info, void * context);
    int active;
    struct sigaction oact;
} rpmsigTbl[] = {
    { SIGINT,	rpmsqAction },
#define	rpmsigTbl_sigint	(&rpmsigTbl[0])
    { SIGQUIT,	rpmsqAction },
#define	rpmsigTbl_sigquit	(&rpmsigTbl[1])
    { SIGCHLD,	rpmsqAction },
#define	rpmsigTbl_sigchld	(&rpmsigTbl[2])
    { SIGHUP,	rpmsqAction },
#define	rpmsigTbl_sighup	(&rpmsigTbl[3])
    { SIGTERM,	rpmsqAction },
#define	rpmsigTbl_sigterm	(&rpmsigTbl[4])
    { SIGPIPE,	rpmsqAction },
#define	rpmsigTbl_sigpipe	(&rpmsigTbl[5])
    { -1,	NULL },
};
/*@=fullinitblock@*/

/*@-incondefs@*/
void rpmsqAction(int signum, siginfo_t * info, void * context)
{
    int save = errno;
    rpmsig tbl;

    for (tbl = rpmsigTbl; tbl->signum >= 0; tbl++) {
	if (tbl->signum != signum)
	    continue;

	(void) sigaddset(&rpmsqCaught, signum);

	switch (signum) {
	case SIGCHLD:
	    while (1) {
		rpmsq sq;
		int status = 0;
		pid_t reaped = waitpid(0, &status, WNOHANG);

		/* XXX errno set to ECHILD/EINVAL/EINTR. */
		if (reaped <= 0)
		    /*@innerbreak@*/ break;

		/* XXX insque(3)/remque(3) are dequeue, not ring. */
		for (sq = rpmsqQueue->q_forw;
		     sq != NULL && sq != rpmsqQueue;
		     sq = sq->q_forw)
		{
		    if (sq->child != reaped)
			/*@innercontinue@*/ continue;
		    sq->reaped = reaped;
		    sq->status = status;
		    (void) pthread_cond_signal(&sq->cond);
		    /*@innerbreak@*/ break;
		}
	    }
	    /*@switchbreak@*/ break;
	default:
	    /*@switchbreak@*/ break;
	}
	break;
    }
    errno = save;
}
/*@=incondefs@*/

int rpmsqEnable(int signum, /*@null@*/ rpmsqAction_t handler)
{
    int tblsignum = (signum >= 0 ? signum : -signum);
    struct sigaction sa;
    rpmsig tbl;
    int ret = -1;

    DO_LOCK ();
    if (rpmsqQueue->id == NULL)
	rpmsqQueue->id = ME();
    for (tbl = rpmsigTbl; tbl->signum >= 0; tbl++) {
	if (tblsignum != tbl->signum)
	    continue;

	if (signum >= 0) {			/* Enable. */
	    if (ADD_REF(tbl) <= 0) {
		(void) sigdelset(&rpmsqCaught, tbl->signum);
		sigemptyset (&sa.sa_mask);
		sa.sa_flags = SA_SIGINFO;
		sa.sa_sigaction = (handler != NULL ? handler : tbl->handler);
		if (sigaction(tbl->signum, &sa, &tbl->oact) < 0) {
		    SUB_REF(tbl);
		    break;
		}
		tbl->active = 1;		/* XXX just in case */
		if (handler != NULL)
		    tbl->handler = handler;
	    }
	} else {				/* Disable. */
	    if (SUB_REF(tbl) <= 0) {
		if (sigaction(tbl->signum, &tbl->oact, NULL) < 0)
		    break;
		tbl->active = 0;		/* XXX just in case */
		tbl->handler = (handler != NULL ? handler : rpmsqAction);
	    }
	}
	ret = tbl->active;
	break;
    }
    DO_UNLOCK ();
    return ret;
}

pid_t rpmsqFork(rpmsq sq)
{
    pid_t pid;
    int xx;

    if (sq->reaper) {
	xx = rpmsqInsert(sq, NULL);
#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "    Enable(%p): %p\n", ME(), sq);
/*@=modfilesys@*/
#endif
	xx = rpmsqEnable(SIGCHLD, NULL);
    }

    xx = pipe(sq->pipes);

    xx = sighold(SIGCHLD);

    pid = fork();
    if (pid < (pid_t) 0) {		/* fork failed.  */
	xx = close(sq->pipes[0]);
	xx = close(sq->pipes[1]);
	sq->pipes[0] = sq->pipes[1] = -1;
	goto out;
    } else if (pid == (pid_t) 0) {	/* Child. */
	int yy;

	/* Block to permit parent to wait. */
	xx = close(sq->pipes[1]);
	xx = read(sq->pipes[0], &yy, sizeof(yy));
	xx = close(sq->pipes[0]);
	sq->pipes[0] = sq->pipes[1] = -1;

#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "     Child(%p): %p child %d\n", ME(), sq, getpid());
/*@=modfilesys@*/
#endif

    } else {				/* Parent. */

	sq->child = pid;

#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "    Parent(%p): %p child %d\n", ME(), sq, sq->child);
/*@=modfilesys@*/
#endif

#ifdef	DYING
	/* Unblock child. */
	xx = close(sq->pipes[0]);
	xx = close(sq->pipes[1]);
	sq->pipes[0] = sq->pipes[1] = -1;
#endif

    }

out:
    xx = sigrelse(SIGCHLD);
    return sq->child;
}

/**
 * Wait for child process to be reaped, and unregister SIGCHLD handler.
 * @param sq		scriptlet queue element
 * @return		0 on success
 */
static int rpmsqWaitUnregister(rpmsq sq)
	/*@globals fileSystem, internalState @*/
	/*@modifies fileSystem, internalState @*/
{
    struct rpmsw_s end;
    int same_thread = 0;
    int ret = 0;
    int xx;

    if (same_thread)
	ret = sighold(SIGCHLD);
    else
	ret = pthread_mutex_lock(&sq->mutex);

    /* Start the child. */
    if (sq->pipes[0] >= 0)
	xx = close(sq->pipes[0]);
    if (sq->pipes[1] >= 0)
	xx = close(sq->pipes[1]);
    sq->pipes[0] = sq->pipes[1] = -1;

    (void) rpmswNow(&sq->begin);

    /*@-infloops@*/
    while (ret == 0 && sq->reaped != sq->child) {
	if (same_thread)
	    ret = sigpause(SIGCHLD);
	else
	    ret = pthread_cond_wait(&sq->cond, &sq->mutex);
    }
    /*@=infloops@*/

    sq->msecs = rpmswDiff(rpmswNow(&end), &sq->begin)/1000;
    sq->script_msecs += sq->msecs;

    if (same_thread)
	xx = sigrelse(SIGCHLD);
    else
	xx = pthread_mutex_unlock(&sq->mutex);

#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "      Wake(%p): %p child %d reaper %d ret %d\n", ME(), sq, sq->child, sq->reaper, ret);
/*@=modfilesys@*/
#endif

    xx = rpmsqRemove(sq);
    xx = rpmsqEnable(-SIGCHLD, NULL);
#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "   Disable(%p): %p\n", ME(), sq);
/*@=modfilesys@*/
#endif

    return ret;
}

pid_t rpmsqWait(rpmsq sq)
{

#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "      Wait(%p): %p child %d reaper %d\n", ME(), sq, sq->child, sq->reaper);
/*@=modfilesys@*/
#endif

    if (sq->reaper) {
	(void) rpmsqWaitUnregister(sq);
    } else {
	pid_t reaped;
	int status;
	do {
	    reaped = waitpid(sq->child, &status, 0);
	} while (reaped >= 0 && reaped != sq->child);
	sq->reaped = reaped;
	sq->status = status;
#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "   Waitpid(%p): %p child %d reaped %d\n", ME(), sq, sq->child, sq->reaped);
/*@=modfilesys@*/
#endif
    }

#ifdef _RPMSQ_DEBUG
/*@-modfilesys@*/
if (_rpmsq_debug)
fprintf(stderr, "      Fini(%p): %p child %d status 0x%x\n", ME(), sq, sq->child, sq->status);
/*@=modfilesys@*/
#endif

    return sq->reaped;
}

int rpmsqThread(void * (*start) (void * arg), void * arg)
{
    pthread_t pth;
    int ret;

    ret = pthread_create(&pth, NULL, start, arg);
    if (ret == 0) {
#if 0
fprintf(stderr, "    Thread(%p): %p\n", ME(), pth);
#endif
	ret = pthread_join(pth, NULL);
    }
    return ret;
}

/**
 * SIGCHLD cancellation handler.
 */
static void
sigchld_cancel (void *arg)
{
    pid_t child = *(pid_t *) arg;
    pid_t result;

    (void) kill(child, SIGKILL);

    do {
	result = waitpid(child, NULL, 0);
    } while (result == (pid_t)-1 && errno == EINTR);

    DO_LOCK ();
    if (SUB_REF (rpmsigTbl_sigchld) == 0) {
	(void) rpmsqEnable(-SIGQUIT, NULL);
	(void) rpmsqEnable(-SIGINT, NULL);
    }
    DO_UNLOCK ();
}

/**
 * Execute a command, returning its status.
 */
int
rpmsqExecve (const char ** argv)
{
    int oldtype;
    int status = -1;
    pid_t pid;
    pid_t result;
    sigset_t newMask, oldMask;
    rpmsq sq = memset(alloca(sizeof(*sq)), 0, sizeof(*sq));

    DO_LOCK ();
    if (ADD_REF (rpmsigTbl_sigchld) == 0) {
	if (rpmsqEnable(SIGINT, NULL) < 0) {
	    SUB_REF (rpmsigTbl_sigchld);
	    goto out;
	}
	if (rpmsqEnable(SIGQUIT, NULL) < 0) {
	    SUB_REF (rpmsigTbl_sigchld);
	    goto out_restore_sigint;
	}
    }
    DO_UNLOCK ();

    sigemptyset (&newMask);
    sigaddset (&newMask, SIGCHLD);
    if (sigprocmask (SIG_BLOCK, &newMask, &oldMask) < 0) {
	DO_LOCK ();
	if (SUB_REF (rpmsigTbl_sigchld) == 0)
	    goto out_restore_sigquit_and_sigint;
	goto out;
    }

    CLEANUP_HANDLER(sigchld_cancel, &pid, &oldtype);

    pid = fork ();
    if (pid < (pid_t) 0) {		/* fork failed.  */
	goto out;
    } else if (pid == (pid_t) 0) {	/* Child. */

	/* Restore the signals.  */
	(void) sigaction (SIGINT, &rpmsigTbl_sigint->oact, NULL);
	(void) sigaction (SIGQUIT, &rpmsigTbl_sigquit->oact, NULL);
	(void) sigprocmask (SIG_SETMASK, &oldMask, NULL);

	/* Reset rpmsigTbl lock and refcnt. */
	INIT_LOCK ();

	(void) execve (argv[0], (char *const *) argv, environ);
	_exit (127);
    } else {				/* Parent. */
	do {
	    result = waitpid(pid, &status, 0);
	} while (result == (pid_t)-1 && errno == EINTR);
	if (result != pid)
	    status = -1;
    }

    CLEANUP_RESET(0, oldtype);

    DO_LOCK ();
    if ((SUB_REF (rpmsigTbl_sigchld) == 0 &&
        (rpmsqEnable(-SIGINT, NULL) < 0 || rpmsqEnable (-SIGQUIT, NULL) < 0))
      || sigprocmask (SIG_SETMASK, &oldMask, NULL) != 0)
    {
	status = -1;
    }
    goto out;

out_restore_sigquit_and_sigint:
    (void) rpmsqEnable(-SIGQUIT, NULL);
out_restore_sigint:
    (void) rpmsqEnable(-SIGINT, NULL);
out:
    DO_UNLOCK ();
    return status;
}
