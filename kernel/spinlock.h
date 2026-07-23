// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

#ifdef LAB_LOCK

struct rwspinlock {

  struct spinlock lock;

  int readers;

  int writer;

  int waiting_writers;

  int writer_pending;

};

#endif
